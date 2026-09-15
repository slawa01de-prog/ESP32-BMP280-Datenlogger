import math
import bisect
import sqlite3
import os
from pathlib import Path
from logger_protocol import parse_packet, packet_key

"""
BMP280 Logger V7.4.2 WebGUI/OTA/MQTT GUI

Kompatibel mit:
- BMP280 Logger V7.4.2 WebGUI OTA MQTT Hybrid

Features:
- Hybrid/Gateway/Sensor Steuerung
- Pairing mit ID + Code
- MultiGraph mit mehreren auswählbaren Modulen
- Analyse pro Modul
- Einstellungen pro direkt verbundenem Modul
- Terminal
- CSV Export
"""

import csv
import datetime as dt
import math
import queue
import threading
import webbrowser
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog

try:
    import serial
    from serial.tools import list_ports
except Exception:
    serial = None
    list_ports = None

try:
    import matplotlib
    matplotlib.use("TkAgg")
    from matplotlib.figure import Figure
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    import matplotlib.ticker as mticker
except Exception:
    Figure = None
    FigureCanvasTkAgg = None
    mticker = None

APP_TITLE = "BMP280 Logger V7.4.2 HA/MQTT UI"
BAUD_DEFAULT = 115200

COL = {
    "bg": "#0b1220",
    "panel": "#111c2e",
    "panel2": "#16243a",
    "border": "#2d3b55",
    "text": "#eaf2ff",
    "muted": "#9fb0c6",
    "accent": "#38bdf8",
    "good": "#22c55e",
    "warn": "#f59e0b",
    "bad": "#ef4444",
    "purple": "#a78bfa",
    "grid": "#263449",
}


LANG_UI = {
    "de": {
        "connected": "VERBUNDEN",
        "not_connected": "NICHT VERBUNDEN",
        "disconnecting": "TRENNE...",
        "connect": "Verbinden",
        "disconnect": "Trennen",
        "status_ready": "Bereit. Hauptfunktionen sind jetzt im Menü oben.",
        "sent": "Gesendet",
        "send_error": "FEHLER beim Senden",
        "connected_ready": "Verbunden. Bereit.",
        "connected_rx": "Verbunden. Datenempfang aktiv.",
        "com_disconnect": "COM getrennt.",
        "stale_disconnect": "Verbunden. Stale Disconnect ignoriert.",
        "last_measurement": "Letzte Messung",
        "interval": "Intervall",
        "storage": "Speicher",
        "nodes": "Nodes",
        "mode": "Mode",
        "power": "Power",
        "device_lang": "Logger Sprache",
        "gui_lang": "GUI Sprache",
        "clear_title": "Local Log löschen",
        "clear_question": "Lokalen Logger-Speicher vom direkt verbundenen Gerät wirklich löschen?\nKalibrierung, Pairing und Einstellungen bleiben erhalten.",
        "no_csv": "keine PC-CSV",
        "csv_inactive": "Keine PC-CSV aktiv",
        "csv_active": "CSV: aktiv",
        "graph_none": "Noch keine Module empfangen.",
        "graph_no_sel": "Keine Module für den Graph ausgewählt.",
        "graph_active": "Aktiv im Graph:",
        "clear_graph_title": "Graph Daten löschen",
        "clear_graph_question": "Graph-Daten der ausgewählten Module löschen?",
        "trust_delete": "Trust löschen",
        "trust_delete_question": "aus Gateway-Whitelist löschen?",
        "enter_id_code": "ID und Code eingeben.",
        "status_updated": "Status aktualisiert",
        "language_status": "Sprache",
    },
    "en": {
        "connected": "CONNECTED",
        "not_connected": "NOT CONNECTED",
        "disconnecting": "DISCONNECTING...",
        "connect": "Connect",
        "disconnect": "Disconnect",
        "status_ready": "Ready. Main functions are now in the top menu.",
        "sent": "Sent",
        "send_error": "SEND ERROR",
        "connected_ready": "Connected. Ready.",
        "connected_rx": "Connected. Receiving data.",
        "com_disconnect": "COM disconnected.",
        "stale_disconnect": "Connected. Stale disconnect ignored.",
        "last_measurement": "Last measurement",
        "interval": "Interval",
        "storage": "Storage",
        "nodes": "Nodes",
        "mode": "Mode",
        "power": "Power",
        "device_lang": "Logger language",
        "gui_lang": "GUI language",
        "clear_title": "Clear local log",
        "clear_question": "Really clear the local log storage of the directly connected device?\nCalibration, pairing and settings stay saved.",
        "no_csv": "no PC CSV",
        "csv_inactive": "No PC CSV active",
        "csv_active": "CSV: active",
        "graph_none": "No modules received yet.",
        "graph_no_sel": "No modules selected for the graph.",
        "graph_active": "Active in graph:",
        "clear_graph_title": "Clear graph data",
        "clear_graph_question": "Clear graph data of the selected modules?",
        "trust_delete": "Delete trust",
        "trust_delete_question": "remove from gateway whitelist?",
        "enter_id_code": "Enter ID and code.",
        "status_updated": "Status updated",
        "language_status": "Language",
    }
}

WIDGET_TEXT = {
    "English": {
        "Verbinden": "Connect", "Trennen": "Disconnect", "Trenne...": "Disconnecting...",
        "Quick Actions": "Quick Actions", "Logging START": "Start logging", "Logging STOP": "Stop logging",
        "1x Loggen": "Log once", "PC-CSV wählen": "Select PC CSV", "Dump Local": "Dump local",
        "Dump Node": "Dump node", "Speicher anzeigen": "Show storage", "Local Log löschen": "Clear local log",
        "Hybrid": "Hybrid", "Sensor": "Sensor", "Gateway": "Gateway", "Status": "Status", "Nodes": "Nodes",
        "Direkt verbundenes Modul steuern": "Control directly connected module", "Einstellungen senden": "Send settings",
        "Display / Modus": "Display / Mode", "Normal": "Normal", "Eco": "Eco", "Deep": "Deep",
        "Start Log": "Start log", "Stop Log": "Stop log", "Storage": "Storage",
        "Intervall s": "Interval s", "Max Tage": "Max days", "Kalibrier Temp": "Calibrate temp",
        "Name": "Name", "Senden": "Send", "Display immer an": "Display always on", "Display 20s": "Display 20s",
        "Wake Display": "Wake display", "USB Live an": "USB live on", "USB Live aus": "USB live off",
        "Graph Druck": "Graph pressure", "Kalibrierung": "Calibration", "Grafiken": "Graphs",
        "Analyse": "Analysis", "Modul-Steuerung": "Module Control", "Export": "Export", "Terminal": "Terminal",
        "Alle Module": "All modules", "Pairing": "Pairing", "PC-CSV wählen": "Select PC CSV",
        "Dump ausgewählte Node": "Dump selected node", "Löschen": "Clear", "Ports": "Ports", "Baud": "Baud",
        "Keine PC-CSV aktiv": "No PC CSV active", "keine PC-CSV": "no PC CSV",
        "GUI Sprache": "GUI language", "Logger Sprache": "Logger language", "Deutsch": "German", "Englisch": "English",
        "Logger DE": "Logger DE", "Logger EN": "Logger EN", "Sprache": "Language",
        "Graph Auswahl": "Graph setup", "Module auswählen:": "Select modules:",
        "Alle anzeigen": "Show all", "Nur ausgewählte Node": "Only selected node",
        "Auswahl leeren": "Clear selection", "Graph Daten löschen": "Clear graph data",
        "Auto-Skalierung": "Auto scale", "Temp von": "Temp from", "Druck von": "Pressure from",
        "bis": "to", "Achsen übernehmen": "Apply axes", "Module im Graph": "Modules in graph",
        "Noch keine Module.": "No modules yet.", "Temperatur Vergleich": "Temperature comparison",
        "Luftdruck Vergleich": "Pressure comparison", "Temperatur mehrerer Module": "Temperature of multiple modules",
        "Luftdruck mehrerer Module": "Pressure of multiple modules", "Punkt": "Sample",
        "Analyse der ausgewählten Node": "Analysis of selected node", "Analyse aktualisieren": "Refresh analysis",
        "Kalibrierung direkt verbundenes Modul": "Calibration of directly connected module",
        "Diese Befehle steuern das Modul, das per USB am PC hängt. Für Batteriemodule ohne USB wird später eine Befehlswarteschlange ergänzt.": "These commands control the module connected by USB to the PC. A command queue for battery modules without USB can be added later.",
        "Letzte Werte vom ausgewählten Modul": "Latest values of selected module", "Noch keine Messdaten.": "No measurement data yet.",
        "Automatisch kalibrieren": "Automatic calibration", "Echte Temperatur °C": "Real temperature °C",
        "Echter Druck hPa": "Real pressure hPa", "cal temp senden": "send cal temp", "cal press senden": "send cal press",
        "Offset manuell setzen": "Set offsets manually", "Temp Offset °C": "Temp offset °C", "Druck Offset hPa": "Pressure offset hPa",
        "Offset setzen": "Set offset", "Schnellaktionen": "Quick actions", "Kalibrier-Seite am Gerät": "Calibration screen on device",
        "Hauptseite am Gerät": "Main screen on device", "Einstellungen senden": "Send settings",
        "Intervall s": "Interval s", "Max Tage": "Max days", "Kalibrier Temp": "Calibrate temp", "Senden": "Send",
        "Display / Modus": "Display / Mode", "Screen Auto": "Screen auto", "Screen Main": "Screen main",
        "Screen Graph": "Screen graph", "Screen Net": "Screen net", "Graph Temp": "Graph temp",
        "Export": "Export", "Keine PC-CSV aktiv": "No PC CSV active", "Löschen": "Clear",
        "Trust löschen": "Delete trust", "Modul hinzufügen": "Add module"
    },
    "Deutsch": {
        "Connect": "Verbinden", "Disconnect": "Trennen", "Disconnecting...": "Trenne...",
        "Start logging": "Logging START", "Stop logging": "Logging STOP", "Log once": "1x Loggen",
        "Select PC CSV": "PC-CSV wählen", "Dump local": "Dump Local", "Dump node": "Dump Node",
        "Show storage": "Speicher anzeigen", "Clear local log": "Local Log löschen",
        "Control directly connected module": "Direkt verbundenes Modul steuern", "Send settings": "Einstellungen senden",
        "Display / Mode": "Display / Modus", "Start log": "Start Log", "Stop log": "Stop Log",
        "Interval s": "Intervall s", "Max days": "Max Tage", "Calibrate temp": "Kalibrier Temp",
        "Send": "Senden", "Display always on": "Display immer an", "Wake display": "Wake Display",
        "USB live on": "USB Live an", "USB live off": "USB Live aus", "Graph pressure": "Graph Druck",
        "Graphs": "Grafiken", "Analysis": "Analyse", "Calibration": "Kalibrierung", "Module Control": "Modul-Steuerung",
        "All modules": "Alle Module", "Dump selected node": "Dump ausgewählte Node", "Clear": "Löschen",
        "No PC CSV active": "Keine PC-CSV aktiv", "no PC CSV": "keine PC-CSV",
        "GUI language": "GUI Sprache", "Logger language": "Logger Sprache", "German": "Deutsch", "English": "Englisch",
        "Language": "Sprache", "Graph setup": "Graph Auswahl", "Select modules:": "Module auswählen:",
        "Show all": "Alle anzeigen", "Only selected node": "Nur ausgewählte Node",
        "Clear selection": "Auswahl leeren", "Clear graph data": "Graph Daten löschen",
        "Auto scale": "Auto-Skalierung", "Temp from": "Temp von", "Pressure from": "Druck von",
        "to": "bis", "Apply axes": "Achsen übernehmen", "Modules in graph": "Module im Graph",
        "No modules yet.": "Noch keine Module.", "Temperature comparison": "Temperatur Vergleich",
        "Pressure comparison": "Luftdruck Vergleich", "Temperature of multiple modules": "Temperatur mehrerer Module",
        "Pressure of multiple modules": "Luftdruck mehrerer Module", "Sample": "Punkt",
        "Analysis of selected node": "Analyse der ausgewählten Node", "Refresh analysis": "Analyse aktualisieren",
        "Calibration of directly connected module": "Kalibrierung direkt verbundenes Modul",
        "These commands control the module connected by USB to the PC. A command queue for battery modules without USB can be added later.": "Diese Befehle steuern das Modul, das per USB am PC hängt. Für Batteriemodule ohne USB wird später eine Befehlswarteschlange ergänzt.",
        "Latest values of selected module": "Letzte Werte vom ausgewählten Modul", "No measurement data yet.": "Noch keine Messdaten.",
        "Automatic calibration": "Automatisch kalibrieren", "Real temperature °C": "Echte Temperatur °C",
        "Real pressure hPa": "Echter Druck hPa", "send cal temp": "cal temp senden", "send cal press": "cal press senden",
        "Set offsets manually": "Offset manuell setzen", "Temp offset °C": "Temp Offset °C", "Pressure offset hPa": "Druck Offset hPa",
        "Set offset": "Offset setzen", "Quick actions": "Schnellaktionen", "Calibration screen on device": "Kalibrier-Seite am Gerät",
        "Main screen on device": "Hauptseite am Gerät", "Screen auto": "Screen Auto", "Screen main": "Screen Main",
        "Screen graph": "Screen Graph", "Screen net": "Screen Net", "Graph temp": "Graph Temp",
        "Delete trust": "Trust löschen", "Add module": "Modul hinzufügen"
    }
}

def mode_name(v):
    try:
        v = int(v)
    except Exception:
        return str(v)
    return {0: "Sensor", 1: "Gateway", 2: "Hybrid"}.get(v, str(v))

def power_name(v):
    try:
        v = int(v)
    except Exception:
        return str(v)
    return {0: "Normal", 1: "Eco", 2: "Deep"}.get(v, str(v))

class SerialWorker:
    def __init__(self, q):
        self.q=q; self.ser=None; self.thread=None; self.running=False
        self.generation=0; self.stop_event=threading.Event()
        self.lock=threading.RLock()

    def emit(self, kind, payload, generation):
        self.q.put((kind,payload,generation))

    def connect(self, port, baud):
        if serial is None:
            raise RuntimeError("pyserial fehlt")
        self.disconnect(notify=False)
        with self.lock:
            self.generation+=1
            generation=self.generation
            ser=serial.Serial(port,baudrate=int(baud),timeout=0.2,write_timeout=1.0)
            self.ser=ser; self.running=True
            stop=threading.Event(); self.stop_event=stop
            self.thread=threading.Thread(target=self.reader,args=(ser,stop,generation),daemon=True)
            self.thread.start()
        self.emit("status",f"Verbunden mit {port}",generation)

    def disconnect(self, notify=True):
        with self.lock:
            ser=self.ser; thread=self.thread; generation=self.generation
            self.stop_event.set(); self.running=False; self.ser=None
        if ser:
            for name in ("cancel_read","cancel_write","close"):
                try: getattr(ser,name)()
                except Exception: pass
        if thread and thread is not threading.current_thread():
            thread.join(timeout=1.0)
        if notify and ser:
            self.emit("disconnected","COM getrennt",generation)

    def is_connected(self):
        return bool(self.running and self.ser and self.ser.is_open and self.thread and self.thread.is_alive())

    def send(self, cmd):
        if not self.is_connected(): raise RuntimeError("Nicht verbunden")
        cmd=cmd.strip()
        if not cmd: return
        with self.lock:
            ser=self.ser
            if ser is None: raise RuntimeError("Nicht verbunden")
            ser.write((cmd+"\n").encode("utf-8"))
        visible=cmd
        if cmd.startswith(("set wifi_pass", "set mqtt_pass", "set webpass")):
            visible=cmd.split(" ",2)[0]+" "+cmd.split(" ",2)[1]+" ***"
        self.emit("tx",visible,self.generation)

    def reader(self, ser, stop, generation):
        buf=b""
        try:
            while not stop.is_set():
                chunk=ser.read(512)
                if not chunk: continue
                buf+=chunk
                while b"\n" in buf:
                    line,buf=buf.split(b"\n",1)
                    if len(line)>2048:
                        self.emit("warning","Zu lange Datenzeile verworfen",generation); continue
                    line=line.decode("utf-8",errors="replace").strip()
                    if line: self.emit("rx",line,generation)
                if len(buf)>4096:
                    buf=b""; self.emit("warning","Empfangspuffer verworfen: Zeilenende fehlt",generation)
        except Exception as e:
            if not stop.is_set(): self.emit("error",str(e),generation)
        finally:
            try: ser.close()
            except Exception: pass
            with self.lock:
                if self.ser is ser:
                    self.ser=None; self.running=False
            self.emit("disconnected","Empfang beendet",generation)


class DarkButton(tk.Button):
    def __init__(self, master, **kw):
        color = kw.pop("color", COL["accent"])
        fg = kw.pop("fg", "#07111f")
        super().__init__(
            master,
            bg=color,
            fg=fg,
            activebackground=color,
            activeforeground=fg,
            relief="flat",
            bd=0,
            padx=10,
            pady=7,
            font=("Segoe UI", 10, "bold"),
            cursor="hand2",
            **kw
        )

class Card(tk.Frame):
    def __init__(self, master, title="", **kw):
        super().__init__(master, bg=COL["panel"], highlightbackground=COL["border"],
                         highlightthickness=1, bd=0, **kw)
        if title:
            tk.Label(self, text=title, bg=COL["panel"], fg=COL["muted"],
                     font=("Segoe UI", 11, "bold")).pack(anchor="w", padx=14, pady=(10, 0))

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("1360x860")
        self.minsize(1180, 760)
        self.configure(bg=COL["bg"])

        self.q = queue.Queue()
        self.serial = SerialWorker(self.q)

        self.nodes = {}
        self.seen_packets=set()
        self.session_origins={}
        self.sync_active=False
        self.loading_history=False
        self.graph_dirty=True
        self.ui_dirty=False
        self.last_rx_time=None
        self.invalid_packets=0
        archive_dir=Path(os.environ.get("LOCALAPPDATA",str(Path.home()))) / "BMP280_Logger_V74"
        archive_dir.mkdir(parents=True,exist_ok=True)
        self.archive=sqlite3.connect(archive_dir / "measurements.sqlite3")
        self.archive.execute("PRAGMA journal_mode=WAL")
        self.archive.execute("CREATE TABLE IF NOT EXISTS samples (key TEXT PRIMARY KEY, node TEXT, stamp TEXT, line TEXT)")
        self.archive.commit()
        self.pending = {}

        self.selected_node = tk.StringVar(value="")
        self.graph_selected_nodes = set()
        self.web_ip = ""
        self.web_host = "bmp280-logger"
        self.web_status_var = tk.StringVar(value="Web: ?")
        self.node_color_map = {}
        self.color_palette = [
            "#38bdf8", "#22c55e", "#f59e0b", "#ef4444",
            "#a78bfa", "#14b8a6", "#f472b6", "#84cc16",
            "#60a5fa", "#fb7185", "#eab308", "#2dd4bf"
        ]

        self.graph_auto_scale = tk.BooleanVar(value=True)
        self.temp_min_var = tk.StringVar(value="20.0")
        self.temp_max_var = tk.StringVar(value="30.0")
        self.press_min_var = tk.StringVar(value="995.0")
        self.press_max_var = tk.StringVar(value="1020.0")

        self.logger_state_var = tk.StringVar(value="LOG: ?")
        self.rec_var = tk.StringVar(value="●")
        self.mode_status_var = tk.StringVar(value="Mode: ?")
        self.power_status_var = tk.StringVar(value="Power: ?")
        self.interval_status_var = tk.StringVar(value="Intervall: ?")
        self.nodes_status_var = tk.StringVar(value="Nodes: 0")
        self.last_status_var = tk.StringVar(value="Letzte Messung: --")
        self.memory_status_var = tk.StringVar(value="Speicher: ?")
        self.device_language_var = tk.StringVar(value="Logger Sprache: ?")
        self.gui_language_var = tk.StringVar(value="Deutsch")
        self.current_gui_language = "Deutsch"

        self.pc_csv_handle = None
        self.pc_csv_writer = None

        self.build_ui()
        self.refresh_ports()
        self.after(120, self.poll)
        self.after(1000, self.graph_tick)
        self.after(1500, self.auto_status_request)
        self.after(500, self.load_archive)

    def graph_tick(self):
        try:
            if self.graph_dirty: self.redraw_graph()
        finally:
            self.after(1000,self.graph_tick)

    def load_archive(self):
        self.loading_history=True
        try:
            lines=self.archive.execute("SELECT stamp,line FROM samples ORDER BY rowid DESC LIMIT 20000").fetchall()
            for stamp,line in reversed(lines):
                try:
                    record=parse_packet(line)
                    if record['extended'] and not record['epoch_ms']:
                        self.session_origins[(record['id'],record['session'])]=dt.datetime.fromisoformat(stamp)-dt.timedelta(milliseconds=record['elapsed_ms'])
                    self.handle_nodedata(line)
                except (ValueError,KeyError,OverflowError): pass
        finally:
            self.loading_history=False
            self.ui_dirty=True

    def export_archive(self):
        path=filedialog.asksaveasfilename(defaultextension=".csv",filetypes=[("CSV","*.csv")])
        if not path: return
        with open(path,"w",newline="",encoding="utf-8-sig") as f:
            writer=csv.writer(f,delimiter=";")
            writer.writerow(["PC_Zeit","Protokoll","ID","Name","Firmware","Log","T_raw_C","T_C","P_raw_hPa","P_hPa","T_Offset","P_Offset","Modus","Power","Wake","MAC","Sequenz","Session","Laufzeit_ms","UTC_ms","Flags","Intervall_s","Pruefsumme"])
            for stamp,line in self.archive.execute("SELECT stamp,line FROM samples ORDER BY stamp"):
                writer.writerow([stamp]+line.split(","))
        messagebox.showinfo("Export","Archiv exportiert")

    def build_menu(self):
        """V6.9: Hauptfunktionen in Menüs/Untermenüs statt alles oben als Button-Reihe."""
        de = self.lang_code() == "de"
        menubar = tk.Menu(self)

        def add_cmd(menu, label_de, label_en, cmd):
            menu.add_command(label=label_de if de else label_en, command=cmd)

        def add_sep(menu):
            menu.add_separator()

        # Datei / File
        file_menu = tk.Menu(menubar, tearoff=0)
        add_cmd(file_menu, "PC-CSV wählen...", "Select PC CSV...", self.choose_pc_csv)
        add_cmd(file_menu, "Terminal öffnen", "Open terminal", lambda: self.show_tab(7))
        add_sep(file_menu)
        add_cmd(file_menu, "Beenden", "Exit", self.on_close)
        menubar.add_cascade(label="Datei" if de else "File", menu=file_menu)

        # Verbindung / Connection
        conn_menu = tk.Menu(menubar, tearoff=0)
        add_cmd(conn_menu, "Ports aktualisieren", "Refresh ports", self.refresh_ports)
        add_cmd(conn_menu, "Verbinden / Trennen", "Connect / Disconnect", self.toggle_connection)
        add_sep(conn_menu)
        add_cmd(conn_menu, "Status abfragen", "Request status", lambda: self.send("status"))
        add_cmd(conn_menu, "Nodes abfragen", "Request nodes", lambda: self.send("nodes"))
        menubar.add_cascade(label="Verbindung" if de else "Connection", menu=conn_menu)

        # Logger
        logger_menu = tk.Menu(menubar, tearoff=0)
        add_cmd(logger_menu, "Logging starten", "Start logging", lambda: self.send("start"))
        add_cmd(logger_menu, "Logging stoppen", "Stop logging", lambda: self.send("stop"))
        add_cmd(logger_menu, "1x loggen", "Log once", lambda: self.send("once"))
        add_sep(logger_menu)
        mode_menu = tk.Menu(logger_menu, tearoff=0)
        add_cmd(mode_menu, "Sensor", "Sensor", lambda: self.send("set mode sensor"))
        add_cmd(mode_menu, "Gateway", "Gateway", lambda: self.send("set mode gateway"))
        add_cmd(mode_menu, "Hybrid", "Hybrid", lambda: self.send("set mode hybrid"))
        logger_menu.add_cascade(label="Modus" if de else "Mode", menu=mode_menu)
        power_menu = tk.Menu(logger_menu, tearoff=0)
        add_cmd(power_menu, "Normal", "Normal", lambda: self.send("set power normal"))
        add_cmd(power_menu, "Eco", "Eco", lambda: self.send("set power eco"))
        add_cmd(power_menu, "Deep Sleep", "Deep sleep", lambda: self.send("set power deep"))
        logger_menu.add_cascade(label="Power", menu=power_menu)
        display_menu = tk.Menu(logger_menu, tearoff=0)
        add_cmd(display_menu, "Display immer an", "Display always on", lambda: self.send("set display_timeout 0"))
        add_cmd(display_menu, "Display 20s", "Display 20s", lambda: self.send("set display_timeout 20"))
        add_cmd(display_menu, "Display wecken", "Wake display", lambda: self.send("wake"))
        logger_menu.add_cascade(label="Display", menu=display_menu)
        menubar.add_cascade(label="Logger", menu=logger_menu)

        # Speicher / Storage
        storage_menu = tk.Menu(menubar, tearoff=0)
        add_cmd(storage_menu, "Speicher anzeigen", "Show storage", lambda: self.send("storage"))
        add_cmd(storage_menu, "Local Log löschen...", "Clear local log...", self.clear_local_log_confirm)
        add_sep(storage_menu)
        add_cmd(storage_menu, "Dump Local", "Dump local", lambda: self.send("dump local"))
        add_cmd(storage_menu, "Dump ausgewählte Node", "Dump selected node", self.dump_selected)
        add_sep(storage_menu)
        add_cmd(storage_menu, "Export-Tab öffnen", "Open export tab", lambda: self.show_tab(6))
        menubar.add_cascade(label="Speicher" if de else "Storage", menu=storage_menu)

        # Netzwerk / Network
        net_menu = tk.Menu(menubar, tearoff=0)
        add_cmd(net_menu, "Pairing AN", "Pairing ON", lambda: self.send("pair on"))
        add_cmd(net_menu, "Pairing AUS", "Pairing OFF", lambda: self.send("pair off"))
        add_cmd(net_menu, "Pending anzeigen", "Show pending", lambda: self.send("pending"))
        add_cmd(net_menu, "Pair-Code vom direkten Modul", "Pair code from local module", lambda: self.send("pair code"))
        add_sep(net_menu)
        add_cmd(net_menu, "Pairing-Tab öffnen", "Open pairing tab", lambda: self.show_tab(1))
        menubar.add_cascade(label="Netzwerk" if de else "Network", menu=net_menu)

        # WebGUI / OTA
        web_menu = tk.Menu(menubar, tearoff=0)
        add_cmd(web_menu, "Web Status", "Web status", lambda: self.send("web status"))
        add_cmd(web_menu, "WebGUI AN", "WebGUI ON", lambda: self.send("set web on"))
        add_cmd(web_menu, "WebGUI AUS", "WebGUI OFF", lambda: self.send("set web off"))
        add_sep(web_menu)
        add_cmd(web_menu, "WLAN Daten setzen...", "Set WiFi credentials...", self.set_wifi_dialog)
        add_cmd(web_menu, "MQTT / Home Assistant setzen...", "Set MQTT / Home Assistant...", self.set_mqtt_dialog)
        add_cmd(web_menu, "MQTT Status", "MQTT status", lambda: self.send("mqtt status"))
        add_cmd(web_menu, "MQTT Discovery senden", "Send MQTT discovery", lambda: self.send("mqtt discovery"))
        add_sep(web_menu)
        add_cmd(web_menu, "WebGUI im Browser öffnen", "Open WebGUI in browser", self.open_webgui)
        add_cmd(web_menu, "Web Passwort setzen...", "Set web password...", self.set_web_login_dialog)
        add_cmd(web_menu, "Web Passwort zurücksetzen", "Reset web password", self.reset_web_password_confirm)
        add_sep(web_menu)
        add_cmd(web_menu, "OTA Update Seite öffnen", "Open OTA update page", self.open_ota_page)
        menubar.add_cascade(label="WebGUI", menu=web_menu)

        # Ansicht / View
        view_menu = tk.Menu(menubar, tearoff=0)
        for idx, de_label, en_label in [
            (0, "Nodes", "Nodes"), (1, "Pairing", "Pairing"), (2, "Grafiken", "Graphs"),
            (3, "Analyse", "Analysis"), (4, "Kalibrierung", "Calibration"),
            (5, "Modul-Steuerung", "Module control"), (6, "Export", "Export"), (7, "Terminal", "Terminal")
        ]:
            add_cmd(view_menu, de_label, en_label, lambda i=idx: self.show_tab(i))
        menubar.add_cascade(label="Ansicht" if de else "View", menu=view_menu)

        # Sprache / Language
        lang_menu = tk.Menu(menubar, tearoff=0)
        gui_menu = tk.Menu(lang_menu, tearoff=0)
        add_cmd(gui_menu, "Deutsch", "German", lambda: self.set_gui_language("Deutsch"))
        add_cmd(gui_menu, "English", "English", lambda: self.set_gui_language("English"))
        lang_menu.add_cascade(label="GUI Sprache" if de else "GUI language", menu=gui_menu)
        dev_menu = tk.Menu(lang_menu, tearoff=0)
        add_cmd(dev_menu, "Logger Deutsch", "Logger German", lambda: self.set_device_language("de"))
        add_cmd(dev_menu, "Logger English", "Logger English", lambda: self.set_device_language("en"))
        lang_menu.add_cascade(label="Logger Sprache" if de else "Logger language", menu=dev_menu)
        menubar.add_cascade(label="Sprache" if de else "Language", menu=lang_menu)

        self.config(menu=menubar)
        self.menu_bar = menubar

    def show_tab(self, index):
        try:
            self.nb.select(index)
        except Exception:
            pass

    def set_gui_language(self, language):
        self.gui_language_var.set(language)
        self.apply_gui_language()

    def build_ui(self):
        self.build_menu()

        header = tk.Frame(self, bg=COL["bg"])
        header.pack(fill="x", padx=14, pady=(12, 8))

        self.title_label = tk.Label(header, text="BMP280 Logger V7.4.2 HA/MQTT UI",
                 bg=COL["bg"], fg=COL["text"], font=("Segoe UI", 22, "bold"))
        self.title_label.pack(side="left")

        self.conn = tk.Label(header, text="NICHT VERBUNDEN", bg=COL["bad"], fg="white",
                             font=("Segoe UI", 11, "bold"), padx=12, pady=8)
        self.conn.pack(side="right")


        top = Card(self)
        top.pack(fill="x", padx=14, pady=(0, 10))

        row = tk.Frame(top, bg=COL["panel"])
        row.pack(fill="x", padx=14, pady=12)

        tk.Label(row, text="COM", bg=COL["panel"], fg=COL["muted"], font=("Segoe UI", 10, "bold")).pack(side="left")
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(row, textvariable=self.port_var, width=34, state="readonly")
        self.port_box.pack(side="left", padx=8)

        DarkButton(row, text="Ports", command=self.refresh_ports, color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=4)

        tk.Label(row, text="Baud", bg=COL["panel"], fg=COL["muted"]).pack(side="left", padx=(14, 4))
        self.baud_var = tk.StringVar(value=str(BAUD_DEFAULT))
        ttk.Entry(row, textvariable=self.baud_var, width=9).pack(side="left")

        self.connect_btn = DarkButton(row, text="Verbinden", command=self.toggle_connection)
        self.connect_btn.pack(side="left", padx=8)


        strip = tk.Frame(top, bg=COL["panel2"], highlightbackground=COL["border"], highlightthickness=1)
        strip.pack(fill="x", padx=14, pady=(0, 8))

        self.rec_label = tk.Label(strip, textvariable=self.rec_var, bg=COL["panel2"], fg=COL["bad"], font=("Segoe UI", 14, "bold"))
        self.rec_label.pack(side="left", padx=(10, 4))

        for var in [self.logger_state_var, self.interval_status_var, self.mode_status_var,
                    self.power_status_var, self.nodes_status_var, self.memory_status_var,
                    self.device_language_var, self.last_status_var]:
            tk.Label(strip, textvariable=var, bg=COL["panel2"], fg=COL["text"],
                     font=("Segoe UI", 10, "bold")).pack(side="left", padx=10, pady=6)

        self.status_msg = tk.Label(
            top,
            text="Bereit. Hauptfunktionen sind jetzt im Menü oben.",
            bg=COL["panel"],
            fg=COL["muted"],
            font=("Segoe UI", 10, "bold"),
            anchor="w"
        )
        self.status_msg.pack(fill="x", padx=14, pady=(0, 10))

        self.nb = ttk.Notebook(self)
        self.nb.pack(fill="both", expand=True, padx=14, pady=(0, 14))

        self.tab_nodes = tk.Frame(self.nb, bg=COL["bg"])
        self.tab_pair = tk.Frame(self.nb, bg=COL["bg"])
        self.tab_graph = tk.Frame(self.nb, bg=COL["bg"])
        self.tab_analysis = tk.Frame(self.nb, bg=COL["bg"])
        self.tab_calibration = tk.Frame(self.nb, bg=COL["bg"])
        self.tab_settings = tk.Frame(self.nb, bg=COL["bg"])
        self.tab_export = tk.Frame(self.nb, bg=COL["bg"])
        self.tab_terminal = tk.Frame(self.nb, bg=COL["bg"])

        self.nb.add(self.tab_nodes, text="Nodes")
        self.nb.add(self.tab_pair, text="Pairing")
        self.nb.add(self.tab_graph, text="Grafiken")
        self.nb.add(self.tab_analysis, text="Analyse")
        self.nb.add(self.tab_calibration, text="Kalibrierung")
        self.nb.add(self.tab_settings, text="Modul-Steuerung")
        self.nb.add(self.tab_export, text="Export")
        self.nb.add(self.tab_terminal, text="Terminal")

        self.build_nodes()
        self.build_pairing()
        self.build_graph()
        self.build_analysis()
        self.build_calibration()
        self.build_settings()
        self.build_export()
        self.build_terminal()
        self.apply_gui_language(initial=True)

    def build_nodes(self):
        card = Card(self.tab_nodes, title="Alle Module")
        card.pack(fill="both", expand=True, pady=8)

        cols = ("id", "name", "fw", "temp", "press", "log", "mode", "power", "last", "mac")
        self.node_table = ttk.Treeview(card, columns=cols, show="headings")
        self.node_table.pack(fill="both", expand=True, padx=12, pady=12)
        self.node_table.bind("<<TreeviewSelect>>", self.on_node_select)

        heads = {
            "id": "ID", "name": "Name", "fw": "FW", "temp": "Temp °C", "press": "Druck hPa",
            "log": "Log", "mode": "Mode", "power": "Power", "last": "Letzter Empfang", "mac": "MAC"
        }
        widths = {"id": 120, "name": 120, "fw": 70, "temp": 90, "press": 100, "log": 70,
                  "mode": 80, "power": 80, "last": 160, "mac": 150}
        for c in cols:
            self.node_table.heading(c, text=heads[c])
            self.node_table.column(c, width=widths[c], anchor="center")

        bottom = tk.Frame(card, bg=COL["panel"])
        bottom.pack(fill="x", padx=12, pady=(0, 12))

        tk.Label(bottom, text="Name:", bg=COL["panel"], fg=COL["text"]).pack(side="left")
        self.rename_var = tk.StringVar()
        ttk.Entry(bottom, textvariable=self.rename_var, width=20).pack(side="left", padx=6)
        DarkButton(bottom, text="Umbenennen am Gateway", command=self.rename_selected, color=COL["accent"]).pack(side="left", padx=4)
        DarkButton(bottom, text="Trust löschen", command=self.remove_selected, color=COL["bad"], fg="white").pack(side="left", padx=4)

    def build_pairing(self):
        top = Card(self.tab_pair, title="Pairing")
        top.pack(fill="x", pady=8)

        row = tk.Frame(top, bg=COL["panel"])
        row.pack(fill="x", padx=16, pady=14)

        DarkButton(row, text="Pairing AN", command=lambda: self.send("pair on"), color=COL["good"]).pack(side="left", padx=4)
        DarkButton(row, text="Pairing AUS", command=lambda: self.send("pair off"), color=COL["warn"]).pack(side="left", padx=4)
        DarkButton(row, text="Pending", command=lambda: self.send("pending"), color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=4)
        DarkButton(row, text="Pair Code vom direkten Modul", command=lambda: self.send("pair code"), color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=4)

        form = Card(self.tab_pair, title="Neues Modul hinzufügen")
        form.pack(fill="x", pady=(0, 8))

        fr = tk.Frame(form, bg=COL["panel"])
        fr.pack(fill="x", padx=16, pady=14)

        tk.Label(fr, text="ID:", bg=COL["panel"], fg=COL["text"], font=("Segoe UI", 10, "bold")).pack(side="left")
        self.pair_id = tk.StringVar()
        ttk.Entry(fr, textvariable=self.pair_id, width=16).pack(side="left", padx=6)

        tk.Label(fr, text="Code:", bg=COL["panel"], fg=COL["text"], font=("Segoe UI", 10, "bold")).pack(side="left", padx=(12, 0))
        self.pair_code = tk.StringVar()
        ttk.Entry(fr, textvariable=self.pair_code, width=10).pack(side="left", padx=6)

        DarkButton(fr, text="Modul hinzufügen", command=self.pair_add, color=COL["accent"]).pack(side="left", padx=8)

        list_card = Card(self.tab_pair, title="Pending Pair Requests")
        list_card.pack(fill="both", expand=True)

        cols = ("id", "fw", "mac", "seen")
        self.pending_table = ttk.Treeview(list_card, columns=cols, show="headings")
        self.pending_table.pack(fill="both", expand=True, padx=12, pady=12)
        self.pending_table.bind("<<TreeviewSelect>>", self.on_pending_select)

        for c, h, w in [("id","ID",140), ("fw","FW",80), ("mac","MAC",160), ("seen","Empfangen",180)]:
            self.pending_table.heading(c, text=h)
            self.pending_table.column(c, width=w, anchor="center")

    def build_graph(self):
        if Figure is None:
            tk.Label(self.tab_graph, text="matplotlib fehlt", bg=COL["bg"], fg=COL["bad"]).pack()
            return

        top = Card(self.tab_graph, title="Graph Auswahl")
        top.pack(fill="x", pady=8)

        row = tk.Frame(top, bg=COL["panel"])
        row.pack(fill="x", padx=14, pady=12)

        tk.Label(row, text="Module auswählen:", bg=COL["panel"], fg=COL["text"],
                 font=("Segoe UI", 10, "bold")).pack(side="left")

        DarkButton(row, text="Alle anzeigen", command=self.select_all_graph_nodes, color=COL["good"]).pack(side="left", padx=4)
        DarkButton(row, text="Nur ausgewählte Node", command=self.select_current_graph_node, color=COL["accent"]).pack(side="left", padx=4)
        DarkButton(row, text="Auswahl leeren", command=self.clear_graph_selection, color=COL["warn"]).pack(side="left", padx=4)
        DarkButton(row, text="Graph Daten löschen", command=self.clear_selected_graph, color=COL["bad"], fg="white").pack(side="left", padx=4)

        axis_row = tk.Frame(top, bg=COL["panel"])
        axis_row.pack(fill="x", padx=14, pady=(0, 12))

        ttk.Checkbutton(
            axis_row,
            text="Auto-Skalierung",
            variable=self.graph_auto_scale,
            command=self.redraw_graph
        ).pack(side="left", padx=(0, 14))

        tk.Label(axis_row, text="Temp von", bg=COL["panel"], fg=COL["muted"]).pack(side="left")
        ttk.Entry(axis_row, textvariable=self.temp_min_var, width=7).pack(side="left", padx=(4, 8))
        tk.Label(axis_row, text="bis", bg=COL["panel"], fg=COL["muted"]).pack(side="left")
        ttk.Entry(axis_row, textvariable=self.temp_max_var, width=7).pack(side="left", padx=(4, 14))

        tk.Label(axis_row, text="Druck von", bg=COL["panel"], fg=COL["muted"]).pack(side="left")
        ttk.Entry(axis_row, textvariable=self.press_min_var, width=8).pack(side="left", padx=(4, 8))
        tk.Label(axis_row, text="bis", bg=COL["panel"], fg=COL["muted"]).pack(side="left")
        ttk.Entry(axis_row, textvariable=self.press_max_var, width=8).pack(side="left", padx=(4, 14))

        DarkButton(axis_row, text="Achsen übernehmen", command=self.apply_manual_axes, color=COL["accent"]).pack(side="left", padx=4)
        DarkButton(axis_row, text="Auto", command=self.set_auto_axes, color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=4)

        main = tk.Frame(self.tab_graph, bg=COL["bg"])
        main.pack(fill="both", expand=True)

        select_card = Card(main, title="Module im Graph")
        select_card.pack(side="left", fill="y", padx=(0, 8), pady=(0, 8))

        self.graph_listbox = tk.Listbox(
            select_card,
            selectmode="multiple",
            bg="#050914",
            fg=COL["text"],
            selectbackground=COL["accent"],
            selectforeground="#07111f",
            relief="flat",
            font=("Consolas", 10),
            width=30,
            height=24,
            exportselection=False
        )
        self.graph_listbox.pack(fill="y", expand=False, padx=12, pady=(12, 6))
        self.graph_listbox.bind("<<ListboxSelect>>", self.on_graph_selection_change)

        self.graph_info_label = tk.Label(
            select_card, text="Noch keine Module.", bg=COL["panel"], fg=COL["muted"],
            font=("Segoe UI", 9), justify="left", wraplength=240
        )
        self.graph_info_label.pack(fill="x", padx=12, pady=(0, 12))

        plot_area = tk.Frame(main, bg=COL["bg"])
        plot_area.pack(side="left", fill="both", expand=True)

        left = Card(plot_area, title="Temperatur Vergleich")
        left.pack(side="left", fill="both", expand=True, padx=(0,7))

        right = Card(plot_area, title="Luftdruck Vergleich")
        right.pack(side="left", fill="both", expand=True, padx=(7,0))

        self.fig_t = Figure(figsize=(5,4), dpi=100, facecolor=COL["panel"])
        self.ax_t = self.fig_t.add_subplot(111)
        self.style_axis(self.ax_t, "Temperatur mehrerer Module", "Punkt", "°C")
        self.canvas_t = FigureCanvasTkAgg(self.fig_t, master=left)
        self.canvas_t.get_tk_widget().pack(fill="both", expand=True, padx=12, pady=12)

        self.fig_p = Figure(figsize=(5,4), dpi=100, facecolor=COL["panel"])
        self.ax_p = self.fig_p.add_subplot(111)
        self.style_axis(self.ax_p, "Luftdruck mehrerer Module", "Punkt", "hPa")
        self.canvas_p = FigureCanvasTkAgg(self.fig_p, master=right)
        self.canvas_p.get_tk_widget().pack(fill="both", expand=True, padx=12, pady=12)

    def build_analysis(self):
        card = Card(self.tab_analysis, title="Analyse der ausgewählten Node")
        card.pack(fill="both", expand=True, pady=8)

        row = tk.Frame(card, bg=COL["panel"])
        row.pack(fill="x", padx=16, pady=12)

        tk.Label(row, text="Node:", bg=COL["panel"], fg=COL["text"]).pack(side="left")
        self.analysis_node = tk.StringVar()
        self.analysis_combo = ttk.Combobox(row, textvariable=self.analysis_node, width=22, state="readonly")
        self.analysis_combo.pack(side="left", padx=8)
        self.analysis_combo.bind("<<ComboboxSelected>>", lambda e: self.update_analysis())
        DarkButton(row, text="Analyse aktualisieren", command=self.update_analysis).pack(side="left", padx=8)

        self.analysis_text = tk.Text(card, bg="#050914", fg=COL["text"], font=("Consolas", 12),
                                     relief="flat", height=22)
        self.analysis_text.pack(fill="both", expand=True, padx=16, pady=(0, 16))


    def build_calibration(self):
        top = Card(self.tab_calibration, title="Kalibrierung direkt verbundenes Modul")
        top.pack(fill="x", pady=8)

        info = tk.Label(
            top,
            text="Diese Befehle steuern das Modul, das per USB am PC hängt. Für Batteriemodule ohne USB wird später eine Befehlswarteschlange ergänzt.",
            bg=COL["panel"],
            fg=COL["muted"],
            font=("Segoe UI", 10),
            wraplength=900,
            justify="left"
        )
        info.pack(fill="x", padx=16, pady=(12, 4))

        live = Card(self.tab_calibration, title="Letzte Werte vom ausgewählten Modul")
        live.pack(fill="x", pady=(0, 8))

        self.cal_info = tk.Label(
            live,
            text="Noch keine Messdaten.",
            bg=COL["panel"],
            fg=COL["text"],
            font=("Consolas", 12),
            justify="left",
            anchor="w"
        )
        self.cal_info.pack(fill="x", padx=16, pady=14)

        cal = Card(self.tab_calibration, title="Automatisch kalibrieren")
        cal.pack(fill="x", pady=(0, 8))

        row = tk.Frame(cal, bg=COL["panel"])
        row.pack(fill="x", padx=16, pady=14)

        self.cal_temp_real = tk.StringVar(value="23.8")
        self.cal_press_real = tk.StringVar(value="1006.9")

        block = tk.Frame(row, bg=COL["panel"])
        block.pack(side="left", padx=8)
        tk.Label(block, text="Echte Temperatur °C", bg=COL["panel"], fg=COL["muted"]).pack(anchor="w")
        ttk.Entry(block, textvariable=self.cal_temp_real, width=14).pack(anchor="w")
        DarkButton(block, text="cal temp senden", command=lambda: self.send(f"cal temp {self.cal_temp_real.get()}"), color=COL["accent"]).pack(anchor="w", pady=4)

        block = tk.Frame(row, bg=COL["panel"])
        block.pack(side="left", padx=8)
        tk.Label(block, text="Echter Druck hPa", bg=COL["panel"], fg=COL["muted"]).pack(anchor="w")
        ttk.Entry(block, textvariable=self.cal_press_real, width=14).pack(anchor="w")
        DarkButton(block, text="cal press senden", command=lambda: self.send(f"cal press {self.cal_press_real.get()}"), color=COL["accent"]).pack(anchor="w", pady=4)

        off = Card(self.tab_calibration, title="Offset manuell setzen")
        off.pack(fill="x", pady=(0, 8))

        row2 = tk.Frame(off, bg=COL["panel"])
        row2.pack(fill="x", padx=16, pady=14)

        self.temp_offset_var = tk.StringVar(value="0.00")
        self.press_offset_var = tk.StringVar(value="0.0")

        block = tk.Frame(row2, bg=COL["panel"])
        block.pack(side="left", padx=8)
        tk.Label(block, text="Temp Offset °C", bg=COL["panel"], fg=COL["muted"]).pack(anchor="w")
        ttk.Entry(block, textvariable=self.temp_offset_var, width=14).pack(anchor="w")
        DarkButton(block, text="Offset setzen", command=lambda: self.send(f"set temp_offset {self.temp_offset_var.get()}"), color=COL["panel2"], fg=COL["text"]).pack(anchor="w", pady=4)

        block = tk.Frame(row2, bg=COL["panel"])
        block.pack(side="left", padx=8)
        tk.Label(block, text="Druck Offset hPa", bg=COL["panel"], fg=COL["muted"]).pack(anchor="w")
        ttk.Entry(block, textvariable=self.press_offset_var, width=14).pack(anchor="w")
        DarkButton(block, text="Offset setzen", command=lambda: self.send(f"set press_offset {self.press_offset_var.get()}"), color=COL["panel2"], fg=COL["text"]).pack(anchor="w", pady=4)

        misc = Card(self.tab_calibration, title="Schnellaktionen")
        misc.pack(fill="x", pady=(0, 8))

        row3 = tk.Frame(misc, bg=COL["panel"])
        row3.pack(fill="x", padx=16, pady=14)

        for text, cmd in [
            ("Status lesen", "status"),
            ("1x messen/loggen", "once"),
            ("Kalibrier-Seite am Gerät", "set screen cal"),
            ("Hauptseite am Gerät", "set screen main"),
        ]:
            DarkButton(row3, text=text, command=lambda c=cmd: self.send(c), color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=4)

    def update_calibration_info(self):
        if not hasattr(self, "cal_info"):
            return

        node_id = self.selected_node.get()
        node = self.nodes.get(node_id)

        if not node:
            self.cal_info.config(text="Noch keine Messdaten.")
            return

        text = (
            f"Node:      {node_id}\\n"
            f"Name:      {node.get('name', node_id)}\\n"
            f"Raw Temp:  {node.get('raw_t', float('nan')):.2f} °C\\n"
            f"Temp:      {node.get('temp', float('nan')):.2f} °C\\n"
            f"T Offset:  {node.get('toff', float('nan')):.2f} °C\\n"
            f"Raw Druck: {node.get('raw_p', float('nan')):.2f} hPa\\n"
            f"Druck:     {node.get('press', float('nan')):.2f} hPa\\n"
            f"P Offset:  {node.get('poff', float('nan')):.2f} hPa\\n"
            f"Letztes:   {node.get('last','--')}"
        )
        self.cal_info.config(text=text)

        if "toff" in node:
            self.temp_offset_var.set(f"{node['toff']:.2f}")
        if "poff" in node:
            self.press_offset_var.set(f"{node['poff']:.2f}")


    def build_settings(self):
        card = Card(self.tab_settings, title="Direkt verbundenes Modul steuern")
        card.pack(fill="x", pady=8)

        row = tk.Frame(card, bg=COL["panel"])
        row.pack(fill="x", padx=16, pady=14)

        commands = [
            ("Normal", "set power normal"),
            ("Eco", "set power eco"),
            ("Deep", "set power deep"),
            ("Start Log", "start"),
            ("Stop Log", "stop"),
            ("1x Log", "once"),
            ("Status", "status"),
            ("Storage", "storage"),
            ("Speicher anzeigen", "storage"),
        ]

        for text, cmd in commands:
            DarkButton(row, text=text, command=lambda c=cmd: self.send(c),
                       color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=3)
        DarkButton(row, text="Local Log löschen", command=self.clear_local_log_confirm,
                   color=COL["bad"], fg="white").pack(side="left", padx=8)
        DarkButton(row, text="Logger DE", command=lambda: self.set_device_language("de"),
                   color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=3)
        DarkButton(row, text="Logger EN", command=lambda: self.set_device_language("en"),
                   color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=3)

        grid = Card(self.tab_settings, title="Einstellungen senden")
        grid.pack(fill="x", pady=(0, 8))

        fr = tk.Frame(grid, bg=COL["panel"])
        fr.pack(fill="x", padx=16, pady=14)

        self.interval_var = tk.StringVar(value="60")
        self.maxdays_var = tk.StringVar(value="7")
        self.caltemp_var = tk.StringVar(value="23.8")
        self.name_var = tk.StringVar(value="Logger")

        for label, var, cmd_prefix in [
            ("Intervall s", self.interval_var, "set interval"),
            ("Max Tage", self.maxdays_var, "set maxdays"),
            ("Kalibrier Temp", self.caltemp_var, "cal temp"),
            ("Name", self.name_var, "set name"),
        ]:
            block = tk.Frame(fr, bg=COL["panel"])
            block.pack(side="left", padx=8)
            tk.Label(block, text=label, bg=COL["panel"], fg=COL["muted"]).pack(anchor="w")
            ttk.Entry(block, textvariable=var, width=12).pack(anchor="w")
            DarkButton(block, text="Senden", command=lambda v=var, p=cmd_prefix: self.send(f"{p} {v.get()}"),
                       color=COL["accent"]).pack(anchor="w", pady=4)

        screen = Card(self.tab_settings, title="Display / Modus")
        screen.pack(fill="x", pady=(0, 8))
        sr = tk.Frame(screen, bg=COL["panel"])
        sr.pack(fill="x", padx=16, pady=14)

        for text, cmd in [
            ("Screen Auto", "set screen auto"),
            ("Screen Main", "set screen main"),
            ("Screen Graph", "set screen graph"),
            ("Screen Net", "set screen net"),
            ("Graph Temp", "set graph temp"),
            ("Graph Druck", "set graph press"),
            ("Display immer an", "set display_timeout 0"),
            ("Display 20s", "set display_timeout 20"),
            ("Wake Display", "wake"),
            ("USB Live an", "set live on"),
            ("USB Live aus", "set live off"),
        ]:
            DarkButton(sr, text=text, command=lambda c=cmd: self.send(c), color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=3)

    def build_export(self):
        DarkButton(self.tab_export,text="Archiv CSV exportieren",command=self.export_archive,color=COL["accent"]).pack(anchor="w",padx=12,pady=6)
        DarkButton(self.tab_export,text="Gespeicherte Logger-Daten nachladen",command=lambda:self.send("sync all"),color=COL["accent"]).pack(anchor="w",padx=12,pady=6)
        card = Card(self.tab_export, title="Export")
        card.pack(fill="x", pady=8)

        row = tk.Frame(card, bg=COL["panel"])
        row.pack(fill="x", padx=16, pady=14)

        DarkButton(row, text="PC-CSV wählen", command=self.choose_pc_csv, color=COL["accent"]).pack(side="left", padx=4)
        self.csv_label = tk.Label(row, text="Keine PC-CSV aktiv", bg=COL["panel"], fg=COL["muted"])
        self.csv_label.pack(side="left", padx=8)

        DarkButton(row, text="Dump ausgewählte Node", command=self.dump_selected, color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=8)
        DarkButton(row, text="Dump Local", command=lambda: self.send("dump local"), color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=4)
        DarkButton(row, text="Speicher anzeigen", command=lambda: self.send("storage"), color=COL["panel2"], fg=COL["text"]).pack(side="left", padx=4)
        DarkButton(row, text="Local Log löschen", command=self.clear_local_log_confirm, color=COL["bad"], fg="white").pack(side="left", padx=4)

    def build_terminal(self):
        card = Card(self.tab_terminal, title="Terminal")
        card.pack(fill="both", expand=True, pady=8)

        self.term = tk.Text(card, bg="#050914", fg=COL["text"], insertbackground=COL["text"],
                            font=("Consolas", 10), relief="flat", wrap="none")
        self.term.pack(fill="both", expand=True, padx=12, pady=(12,8))

        row = tk.Frame(card, bg=COL["panel"])
        row.pack(fill="x", padx=12, pady=(0,12))

        self.cmd_var = tk.StringVar()
        entry = ttk.Entry(row, textvariable=self.cmd_var)
        entry.pack(side="left", fill="x", expand=True)
        entry.bind("<Return>", lambda e: self.send_manual())

        DarkButton(row, text="Senden", command=self.send_manual).pack(side="left", padx=8)
        DarkButton(row, text="Löschen", command=lambda: self.term.delete("1.0","end"), color=COL["panel2"], fg=COL["text"]).pack(side="left")

    def style_axis(self, ax, title, xlabel, ylabel):
        bg = "#0a1220"
        ax.set_facecolor(bg)
        ax.set_title(title, color=COL["text"], fontsize=13, fontweight="bold", pad=12)
        ax.set_xlabel(xlabel, color=COL["muted"], fontsize=10, labelpad=10)
        ax.set_ylabel(ylabel, color=COL["muted"], fontsize=10, labelpad=10)
        ax.tick_params(colors=COL["muted"], labelsize=9)
        for side in ["top", "right"]:
            ax.spines[side].set_visible(False)
        ax.spines["left"].set_color(COL["border"])
        ax.spines["bottom"].set_color(COL["border"])
        ax.grid(True, color=COL["grid"], linewidth=0.9, alpha=0.85)
        try:
            ax.minorticks_on()
            ax.grid(True, which="minor", color=COL["grid"], linewidth=0.4, alpha=0.35)
        except Exception:
            pass
        if mticker:
            ax.yaxis.set_major_formatter(mticker.FormatStrFormatter("%.1f"))
        try:
            ax.margins(x=0.02)
        except Exception:
            pass

    def update_top_status_from_node(self, node):
        self.nodes_status_var.set(f"Nodes: {len(self.nodes)}")
        self.last_status_var.set(
            f"{self.tr('last_measurement')}: {node.get('name', node.get('id', '?'))} "
            f"{node.get('temp', float('nan')):.1f}°C / {node.get('press', float('nan')):.1f} hPa"
        )
        self.mode_status_var.set(f"{self.tr('mode')}: {self.ui_mode_name(node.get('mode', '?'))}")
        self.power_status_var.set(f"{self.tr('power')}: {self.ui_power_name(node.get('power', '?'))}")

    def set_logging_state(self, state):
        state = state.upper()
        if state == "START":
            self.logger_state_var.set("LOG: AKTIV")
            self.rec_label.config(fg=COL["bad"])
        elif state == "STOP":
            self.logger_state_var.set("LOG: STOP")
            self.rec_label.config(fg=COL["muted"])
        else:
            self.logger_state_var.set(f"LOG: {state}")

    def update_interval_from_command(self, cmd):
        parts = cmd.strip().split()
        if len(parts) >= 3 and parts[0] == "set" and parts[1] == "interval":
            self.interval_status_var.set(f"{self.tr('interval')}: {parts[2]} s")


    def auto_status_request(self):
        # V6.7/V6.8: Statusdaten regelmäßig anfordern, damit Intervall/Speicher/Logging oben aktuell bleiben.
        try:
            if self.serial.is_connected():
                self.serial.send("status")
        except Exception:
            pass
        self.after(5000, self.auto_status_request)


    def lang_code(self):
        return "en" if self.gui_language_var.get() == "English" else "de"

    def tr(self, key):
        return LANG_UI.get(self.lang_code(), LANG_UI["de"]).get(key, key)

    def ui_mode_name(self, v):
        raw = mode_name(v)
        mapping = {"Sensor": "Sensor", "Gateway": "Gateway", "Hybrid": "Hybrid"}
        return mapping.get(raw, raw)

    def ui_power_name(self, v):
        raw = power_name(v)
        if self.lang_code() == "en":
            mapping = {"Normal": "Normal", "Eco": "Eco", "Deep": "Deep Sleep"}
        else:
            mapping = {"Normal": "Normal", "Eco": "Eco", "Deep": "Deep"}
        return mapping.get(raw, raw)

    def refresh_graph_titles(self):
        if hasattr(self, "ax_t"):
            self.style_axis(self.ax_t, "Temperature of multiple modules" if self.lang_code()=="en" else "Temperatur mehrerer Module", "Time (unsynced: estimated)" if self.lang_code()=="en" else "Zeit (ohne Uhrsync: geschaetzt)", "°C")
        if hasattr(self, "ax_p"):
            self.style_axis(self.ax_p, "Pressure of multiple modules" if self.lang_code()=="en" else "Luftdruck mehrerer Module", "Time (unsynced: estimated)" if self.lang_code()=="en" else "Zeit (ohne Uhrsync: geschaetzt)", "hPa")

    def translate_widget_tree(self, widget):
        target = self.gui_language_var.get()
        mapping = WIDGET_TEXT.get(target, {})
        try:
            txt = widget.cget("text")
            if txt in mapping:
                widget.config(text=mapping[txt])
        except Exception:
            pass
        for child in widget.winfo_children():
            self.translate_widget_tree(child)

    def apply_gui_language(self, initial=False):
        self.current_gui_language = self.gui_language_var.get()
        self.build_menu()
        self.translate_widget_tree(self)
        try:
            tabs = ["Nodes", "Pairing", "Grafiken", "Analyse", "Kalibrierung", "Modul-Steuerung", "Export", "Terminal"]
            tabs_en = ["Nodes", "Pairing", "Graphs", "Analysis", "Calibration", "Module Control", "Export", "Terminal"]
            for idx, label in enumerate(tabs_en if self.lang_code() == "en" else tabs):
                self.nb.tab(idx, text=label)
        except Exception:
            pass
        if not self.serial.is_connected():
            self.conn.config(text=self.tr("not_connected"))
            self.connect_btn.config(text=self.tr("connect"))
        try:
            self.title_label.config(text="BMP280 Logger V7.4.2 HA/MQTT UI")
        except Exception:
            pass
        self.status_msg.config(text=self.tr("status_ready") if initial else f"{self.tr('gui_lang')}: {self.gui_language_var.get()}")
        self.refresh_graph_titles()
        # Nur Platzhalter neu setzen, wenn noch keine echten Werte da sind.
        if "?" in self.interval_status_var.get():
            self.interval_status_var.set(f"{self.tr('interval')}: ?")
        if "?" in self.memory_status_var.get() or self.memory_status_var.get().startswith("CSV"):
            self.memory_status_var.set(f"{self.tr('storage')}: ?")
        if "?" in self.mode_status_var.get():
            self.mode_status_var.set(f"{self.tr('mode')}: ?")
        if "?" in self.power_status_var.get():
            self.power_status_var.set(f"{self.tr('power')}: ?")
        if "?" in self.device_language_var.get():
            self.device_language_var.set(f"{self.tr('device_lang')}: ?")
        if self.last_status_var.get().endswith("--"):
            self.last_status_var.set(f"{self.tr('last_measurement')}: --")

    def set_device_language(self, lang):
        lang = "en" if str(lang).lower().startswith("en") else "de"
        self.send(f"set lang {lang}")
        self.after(300, lambda: self.send("status") if self.serial.is_connected() else None)

    def open_webgui(self):
        target = self.web_ip.strip()
        if not target:
            messagebox.showinfo("WebGUI", "Bitte zuerst den Logger verbinden und Web Status abrufen. Alternativ die IP-Adresse im Browser eingeben.")
            return
        if "STA " in target:
            target = target.split("STA ", 1)[1].split(" | ", 1)[0]
        elif "AP " in target:
            target = target.split("AP ", 1)[1].split(" | ", 1)[0]
        webbrowser.open(f"http://{target}")

    def open_ota_page(self):
        target = self.web_ip.strip()
        if not target:
            messagebox.showinfo("WebGUI", "Bitte zuerst den Logger verbinden und Web Status abrufen. Alternativ die IP-Adresse im Browser eingeben.")
            return
        if "STA " in target:
            target = target.split("STA ", 1)[1].split(" | ", 1)[0]
        elif "AP " in target:
            target = target.split("AP ", 1)[1].split(" | ", 1)[0]
        webbrowser.open(f"http://{target}/update")


    def set_wifi_dialog(self):
        ssid = simpledialog.askstring("WLAN", "WLAN SSID:", parent=self)
        if ssid is None:
            return
        password = simpledialog.askstring("WLAN", "WLAN Passwort:", show="*", parent=self)
        if password is None:
            return
        host = simpledialog.askstring("WLAN", "WLAN-Hostname (Zugriff per IP):", initialvalue=self.web_host or "bmp280-logger", parent=self)
        if host is None:
            host = "bmp280-logger"
        self.send(f"set wifi_ssid {ssid}")
        self.after(150, lambda: self.send(f"set wifi_pass {password}"))
        self.after(300, lambda: self.send(f"set webhost {host}"))
        self.after(550, lambda: self.send("web status"))
        messagebox.showinfo("WLAN", "WLAN-Daten wurden an den Logger gesendet. Falls er noch im AP ist: Logger neu starten oder Web Status prüfen.")

    def set_mqtt_dialog(self):
        host = simpledialog.askstring("MQTT / Home Assistant", "MQTT Broker IP/Host:", parent=self)
        if host is None:
            return
        port = simpledialog.askstring("MQTT / Home Assistant", "MQTT Port:", initialvalue="1883", parent=self)
        if port is None:
            return
        user = simpledialog.askstring("MQTT / Home Assistant", "MQTT Benutzer (leer wenn keiner):", parent=self) or ""
        password = simpledialog.askstring("MQTT / Home Assistant", "MQTT Passwort:", show="*", parent=self) or ""
        prefix = simpledialog.askstring("MQTT / Home Assistant", "Topic Prefix:", initialvalue="bmp280", parent=self) or "bmp280"
        self.send("set mqtt on")
        self.after(120, lambda: self.send(f"set mqtt_host {host}"))
        self.after(240, lambda: self.send(f"set mqtt_port {port}"))
        self.after(360, lambda: self.send(f"set mqtt_user {user}"))
        self.after(480, lambda: self.send(f"set mqtt_pass {password}"))
        self.after(600, lambda: self.send(f"set mqtt_prefix {prefix}"))
        self.after(760, lambda: self.send("set mqtt_discovery on"))
        self.after(1000, lambda: self.send("mqtt discovery"))
        self.after(1250, lambda: self.send("mqtt status"))
        messagebox.showinfo("MQTT", "MQTT-Daten wurden gesendet. Home Assistant Discovery wird danach automatisch ausgelöst.")

    def set_web_login_dialog(self):
        user = simpledialog.askstring("Web Login", "Benutzername:", initialvalue="admin", parent=self)
        if user is None:
            return
        password = simpledialog.askstring("Web Login", "Passwort:", initialvalue="admin", show="*", parent=self)
        if password is None:
            return
        self.send(f"set webuser {user}")
        self.after(150, lambda: self.send(f"set webpass {password}"))
        self.after(350, lambda: self.send("web status"))

    def reset_web_password_confirm(self):
        if messagebox.askyesno("Web Passwort", "Web Login wirklich auf admin/admin zurücksetzen?"):
            self.send("web resetpass")
            self.after(300, lambda: self.send("web status"))

    def clear_local_log_confirm(self):
        if messagebox.askyesno(self.tr("clear_title"), self.tr("clear_question")):
            self.send("clear")
            self.after(500, lambda: self.send("storage") if self.serial.is_connected() else None)

    def refresh_ports(self):
        if list_ports is None:
            return
        vals = [f"{p.device} - {p.description}" for p in list_ports.comports()]
        self.port_box["values"] = vals
        if vals and not self.port_var.get():
            self.port_var.set(vals[0])

    def selected_port(self):
        txt = self.port_var.get()
        if not txt:
            return None
        return txt.split(" - ")[0]

    def toggle_connection(self):
        if self.serial.is_connected():
            # V6.5: Trennen im Hintergrund, damit die GUI nicht einfriert.
            self.connect_btn.config(state="disabled", text=self.tr("disconnecting"), bg=COL["warn"], fg="#07111f")
            self.conn.config(text=self.tr("disconnecting"), bg=COL["warn"], fg="#07111f")
            self.status_msg.config(text="COM-Port wird getrennt..." if self.lang_code()=="de" else "Disconnecting COM port...", fg=COL["warn"])
            threading.Thread(target=self.serial.disconnect, daemon=True).start()
            return
        try:
            self.serial.connect(self.selected_port(), int(self.baud_var.get()))
            self.conn.config(text=f"{self.tr('connected')} {self.selected_port()}", bg=COL["good"], fg="#04140a")
            self.connect_btn.config(text=self.tr("disconnect"), bg=COL["bad"], fg="white", state="normal")
            self.after(350, lambda: self.send("status") if self.serial.is_connected() else None)
            self.after(650, lambda: self.send(f"time {int(dt.datetime.now().timestamp())}") if self.serial.is_connected() else None)
            self.after(900, lambda: self.send("sync all") if self.serial.is_connected() else None)
            self.status_msg.config(text=self.tr("connected_ready"), fg=COL["good"])
        except Exception as e:
            self.connect_btn.config(state="normal")
            messagebox.showerror("Fehler", str(e))

    def send(self, cmd):
        try:
            self.serial.send(cmd)
            self.status_msg.config(text=self.tr("sent"), fg=COL["text"])
        except Exception as e:
            self.write_term(f"[FEHLER] {e}\n")
            self.status_msg.config(text=f"{self.tr('send_error')}: {e}", fg=COL["bad"])

    def send_manual(self):
        cmd = self.cmd_var.get().strip()
        if not cmd:
            return
        self.send(cmd)
        self.cmd_var.set("")

    def poll(self):
        try:
            for _ in range(100):
                try: event=self.q.get_nowait()
                except queue.Empty: break
                kind,payload=event[:2]
                if len(event)>2 and event[2]!=self.serial.generation: continue
                try:
                    if kind=="rx":
                        self.last_rx_time=dt.datetime.now()
                        self.handle_line(payload)
                    elif kind=="status":
                        self.write_term(f"[STATUS] {payload}\n")
                    elif kind=="tx":
                        self.write_term(f">>> {payload}\n")
                    elif kind in ("error","warning","disconnected"):
                        self.write_term(f"[{kind.upper()}] {payload}\n")
                        if kind=="disconnected": self.sync_active=False
                except Exception as e:
                    self.invalid_packets+=1
                    self.write_term(f"[DATENFEHLER {self.invalid_packets}] {type(e).__name__}: {e}\n")
            if self.ui_dirty:
                self.ui_dirty=False
                self.refresh_node_table(); self.update_node_lists(); self.update_analysis()
                if self.selected_node.get(): self.update_calibration_info()
            self.archive.commit()
            self.sync_connection_indicator()
        finally:
            self.after(120,self.poll)

    def sync_connection_indicator(self):
        if self.serial.is_connected():
            self.conn.config(text=f"{self.tr('connected')} {self.selected_port()}",bg=COL["good"],fg="#04140a")
            self.connect_btn.config(text=self.tr("disconnect"),bg=COL["bad"],fg="white",state="normal")
            if self.last_rx_time and (dt.datetime.now()-self.last_rx_time).total_seconds()>15:
                self.conn.config(text="COM offen - keine Antwort" if self.lang_code()=="de" else "COM open - no response",bg=COL["warn"])
        else:
            self.conn.config(text=self.tr("not_connected"),bg=COL["bad"],fg="white")
            self.connect_btn.config(text=self.tr("connect"),bg=COL["accent"],fg="#07111f",state="normal")

    def handle_line(self, line):
        if not line.startswith("NODEDATA,"): self.write_term(line + "\n")
        if line.startswith("SYNC,"):
            self.sync_active=line=="SYNC,START"
            self.status_msg.config(text="Gespeicherte Messungen werden nachgeladen" if self.sync_active else "Nachladen abgeschlossen")
        elif line.startswith("PAIRREQ,"):
            self.handle_pairreq(line)
        elif line.startswith("PAIRCODE,"):
            parts = line.split(",")
            if len(parts) >= 3:
                self.pair_id.set(parts[1])
                self.pair_code.set(parts[2])
        elif line.startswith("PAIRED,"):
            self.write_term("[INFO] Pairing abgeschlossen\n")
            self.send("trust list")
        elif line.startswith("NODEDATA,"):
            self.handle_nodedata(line)
        elif line.startswith("STATUSDATA,"):
            self.handle_statusdata(line)
        elif line.startswith("WEB,"):
            self.handle_web_line(line)
        elif line.startswith("MQTT,"):
            self.status_msg.config(text=line)
        elif line.startswith("ACK,"):
            self.handle_ack(line)
        elif line.startswith("ERR,"):
            self.handle_err(line)
        elif line.startswith("IGNORED,"):
            self.write_term("[WARN] Unbekannter Node ignoriert\n")



    def handle_web_line(self, line):
        # WEB,on,STA 192.168.x.x | AP 192.168.4.1,host,bmp280-logger,user,admin
        parts = line.split(",")
        if len(parts) >= 3:
            self.web_status_var.set("Web: " + parts[1].upper() + " " + parts[2])
            self.web_ip = parts[2]
        if len(parts) >= 5 and parts[3] == "host":
            self.web_host = parts[4]

    def handle_statusdata(self, line):
        parts = line.split(",")
        # STATUSDATA,nodeid,name,fw,mode,power,logging,interval,maxdays,logcount,maxsamples,usedkb,totalkb,localkb,paired,pairmode,trustedCount[,lang]
        if len(parts) < 17:
            return
        try:
            node_id = parts[1]
            self.local_node_id=node_id
            name = parts[2]
            fw = parts[3]
            mode = int(float(parts[4]))
            power = int(float(parts[5]))
            logging_active = int(float(parts[6])) == 1
            interval = int(float(parts[7]))
            maxdays = float(parts[8])
            logcount = int(float(parts[9]))
            maxsamples = int(float(parts[10]))
            used_kb = float(parts[11])
            total_kb = float(parts[12])
            local_kb = float(parts[13])
            paired = int(float(parts[14])) == 1
            pairmode = int(float(parts[15])) == 1
            trusted_count = int(float(parts[16]))
            device_lang = parts[17].strip().lower() if len(parts) > 17 else ""
            web_on = parts[18].strip() == "1" if len(parts) > 18 else False
            web_ip = parts[19].strip() if len(parts) > 19 else ""
            web_host = parts[20].strip() if len(parts) > 20 else "bmp280-logger"
        except Exception:
            return

        self.logger_state_var.set("LOG: AKTIV" if logging_active else "LOG: STOP")
        self.rec_label.config(fg=COL["bad"] if logging_active else COL["muted"])
        self.interval_status_var.set(f"{self.tr('interval')}: {interval} s")
        self.mode_status_var.set(f"{self.tr('mode')}: {mode_name(mode)}")
        self.power_status_var.set(f"{self.tr('power')}: {power_name(power)}")
        self.nodes_status_var.set(f"{self.tr('nodes')}: {max(len(self.nodes), trusted_count)}")
        if device_lang:
            self.device_language_var.set(f"{self.tr('device_lang')}: {device_lang.upper()}")
        self.web_ip = web_ip
        self.web_host = web_host or "bmp280-logger"
        self.web_status_var.set(f"Web: {'ON' if web_on else 'OFF'} {web_ip}".strip())

        pct = 0.0
        if total_kb > 0:
            pct = (used_kb / total_kb) * 100.0
        self.memory_status_var.set(f"{self.tr('storage')}: {used_kb:.1f}/{total_kb:.0f} KB ({pct:.0f}%)")

        # Die direkt verbundene Node auch in der Tabelle bekannt halten, selbst wenn gerade keine NODEDATA-Zeile kam.
        if node_id and node_id not in self.nodes:
            self.nodes[node_id] = {
                "id": node_id,
                "name": name or node_id,
                "fw": fw,
                "mac": "--",
                "rows": [],
                "log": logcount,
                "mode": mode,
                "power": power,
                "last": "Status",
            }
            self.refresh_node_table()
            self.update_node_lists()
        elif node_id in self.nodes:
            self.nodes[node_id].update({"name": name or node_id, "fw": fw, "log": logcount, "mode": mode, "power": power})
            self.refresh_node_table()

        self.status_msg.config(
            text=f"{self.tr('status_updated')}: {name or node_id} | {self.tr('interval')} {interval}s | Log {logcount}/{maxsamples} | Local {local_kb:.1f}KB",
            fg=COL["good"]
        )

    def handle_ack(self, line):
        parts = line.split(",", 2)
        msg = line
        if len(parts) >= 3:
            msg = f"OK: {parts[1]} -> {parts[2]}"
            cmd = parts[1].strip().lower()
            detail = parts[2].strip().lower()
            if cmd == "start":
                self.set_logging_state("START")
            elif cmd == "stop":
                self.set_logging_state("STOP")
            elif cmd.startswith("set interval"):
                self.update_interval_from_command(cmd)
            elif cmd.startswith("set power"):
                self.power_status_var.set(f"{self.tr('power')}: {detail}")
            elif cmd.startswith("set mode"):
                self.mode_status_var.set(f"{self.tr('mode')}: {detail}")
            elif cmd.startswith("set lang"):
                self.device_language_var.set(f"{self.tr('device_lang')}: {detail.upper()}")
                self.after(250, lambda: self.send("status") if self.serial.is_connected() else None)
            elif cmd == "clear":
                self.after(250, lambda: self.send("storage") if self.serial.is_connected() else None)
        self.status_msg.config(text=msg, fg=COL["good"])

    def handle_err(self, line):
        parts = line.split(",", 2)
        msg = line
        if len(parts) >= 3:
            msg = f"FEHLER: {parts[1]} -> {parts[2]}"
        self.status_msg.config(text=msg, fg=COL["bad"])

    def handle_pairreq(self, line):
        parts = line.split(",")
        if len(parts) < 4:
            return
        node_id, fw, mac = parts[1], parts[2], parts[3]
        self.pending[node_id] = {"id": node_id, "fw": fw, "mac": mac, "seen": dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
        self.refresh_pending_table()
        self.pair_id.set(node_id)

    def handle_nodedata(self, line):
        p=parse_packet(line)
        key=packet_key(p)
        if key in self.seen_packets: return
        if not p["valid"]:
            self.status_msg.config(text=f"Sensorfehler: {p['id']}",fg=COL["bad"])
            return
        now=dt.datetime.now()
        origin_key=(p["id"],p["session"])
        if p["epoch_ms"]:
            stamp=dt.datetime.fromtimestamp(p["epoch_ms"]/1000)
            time_quality="UTC"
        elif p["extended"]:
            origin=self.session_origins.setdefault(origin_key,now-dt.timedelta(milliseconds=p["elapsed_ms"]))
            stamp=origin+dt.timedelta(milliseconds=p["elapsed_ms"])
            time_quality="estimated"
        else:
            stamp=now; time_quality="received"
        n=self.nodes.setdefault(p["id"],{"id":p["id"],"rows":[]})
        # New rows have durable identity across reconnects; old V73 packets use legacy identity.
        if not self.loading_history:
            cursor=self.archive.execute("INSERT OR IGNORE INTO samples VALUES (?,?,?,?)",(key,p["id"],stamp.isoformat(),line))
            if cursor.rowcount==0: return
        if len(self.seen_packets)>100000: self.seen_packets.clear()
        self.seen_packets.add(key)
        row={"dt":stamp,"log":p["log"],"raw_t":p["raw_t"],"temp":p["temp"],"raw_p":p["raw_p"],"press":p["press"],
             "toff":p["toff"],"poff":p["poff"],"mode":p["mode"],"power":p["power"],"wake":p["wake"],
             "session":p["session"],"sequence":p["sequence"],"elapsed_ms":p["elapsed_ms"],"time_quality":time_quality,"interval":p["interval"]}
        if not n["rows"] or n["rows"][-1]["dt"]<=row["dt"]: n["rows"].append(row)
        else: bisect.insort(n["rows"],row,key=lambda r:r["dt"])
        if len(n["rows"])>20000: n["rows"]=n["rows"][-20000:]
        latest=n["rows"][-1]
        n.update({"name":p["name"],"fw":p["fw"],"mac":p["mac"],"last":latest["dt"].strftime("%Y-%m-%d %H:%M:%S")})
        n.update({k:latest[k] for k in ("log","raw_t","temp","raw_p","press","toff","poff","mode","power","wake")})
        if not self.loading_history:
            self.update_top_status_from_node(n)
            if self.pc_csv_writer:
                self.pc_csv_writer.writerow([stamp.isoformat(),p["id"],p["name"],p["fw"],p["log"],p["raw_t"],p["temp"],p["raw_p"],p["press"],p["toff"],p["poff"],p["mode"],p["power"],p["wake"],p["mac"]])
                self.pc_csv_handle.flush()
        self.graph_dirty=True; self.ui_dirty=True

    def refresh_pending_table(self):
        for i in self.pending_table.get_children():
            self.pending_table.delete(i)
        for node_id, p in self.pending.items():
            self.pending_table.insert("", "end", iid=node_id, values=(p["id"], p["fw"], p["mac"], p["seen"]))

    def refresh_node_table(self):
        for i in self.node_table.get_children():
            self.node_table.delete(i)
        for node_id, n in sorted(self.nodes.items()):
            vals = (
                node_id, n.get("name", node_id), n.get("fw","--"),
                f'{n.get("temp", float("nan")):.2f}', f'{n.get("press", float("nan")):.2f}',
                n.get("log","--"), mode_name(n.get("mode","--")), power_name(n.get("power","--")),
                n.get("last","--"), n.get("mac","--")
            )
            self.node_table.insert("", "end", iid=node_id, values=vals)

    def update_node_lists(self):
        ids = sorted(self.nodes.keys())
        if ids and not self.selected_node.get():
            self.selected_node.set(ids[0])

        if hasattr(self, "analysis_combo"):
            self.analysis_combo["values"] = ids
            if ids and not self.analysis_node.get():
                self.analysis_node.set(ids[0])

        self.update_graph_listbox()

    def on_pending_select(self, _):
        sel = self.pending_table.selection()
        if sel:
            self.pair_id.set(sel[0])

    def on_node_select(self, _):
        sel = self.node_table.selection()
        if sel:
            self.selected_node.set(sel[0])
            self.analysis_node.set(sel[0])
            self.rename_var.set(self.nodes.get(sel[0], {}).get("name", sel[0]))
            self.update_analysis()
            self.update_calibration_info()

    def pair_add(self):
        node_id = self.pair_id.get().strip().upper()
        code = self.pair_code.get().strip()
        if len(node_id) != 10 or not code:
            messagebox.showerror("Fehler", self.tr("enter_id_code"))
            return
        self.send(f"pair add {node_id} {code}")

    def rename_selected(self):
        node_id = self.selected_node.get()
        name = self.rename_var.get().strip()
        if not node_id or not name:
            return
        if node_id in self.nodes:
            self.nodes[node_id]["name"] = name
        self.send(f"rename {node_id} {name}")
        self.refresh_node_table()

    def remove_selected(self):
        node_id = self.selected_node.get()
        if not node_id:
            return
        if messagebox.askyesno(self.tr("trust_delete"), f"{node_id} {self.tr('trust_delete_question')}"):
            self.send(f"trust remove {node_id}")

    def node_color(self, node_id):
        if node_id not in self.node_color_map:
            idx = len(self.node_color_map) % len(self.color_palette)
            self.node_color_map[node_id] = self.color_palette[idx]
        return self.node_color_map[node_id]

    def update_graph_listbox(self):
        if not hasattr(self, "graph_listbox"):
            return

        old_selection = set(self.graph_selected_nodes)
        self.graph_listbox.delete(0, "end")

        ids = sorted(self.nodes.keys())
        for node_id in ids:
            name = self.nodes.get(node_id, {}).get("name", node_id)
            self.graph_listbox.insert("end", f"{node_id}  {name}")

        if ids and not old_selection:
            old_selection.add(ids[0])

        self.graph_selected_nodes = {n for n in old_selection if n in self.nodes}

        for idx, node_id in enumerate(ids):
            if node_id in self.graph_selected_nodes:
                self.graph_listbox.selection_set(idx)

        self.update_graph_info_label()

    def update_graph_info_label(self):
        if not hasattr(self, "graph_info_label"):
            return
        selected = self.get_graph_selected_nodes()
        if not self.nodes:
            self.graph_info_label.config(text=self.tr("graph_none"))
            return
        if not selected:
            self.graph_info_label.config(text=self.tr("graph_no_sel"))
            return
        text = "Aktiv im Graph:\n"
        for node_id in selected:
            name = self.nodes.get(node_id, {}).get("name", node_id)
            text += f"■ {node_id} {name}\n"
        self.graph_info_label.config(text=text.strip())

    def get_graph_selected_nodes(self):
        if not hasattr(self, "graph_listbox"):
            return []
        ids = sorted(self.nodes.keys())
        selected = []
        for idx in self.graph_listbox.curselection():
            if 0 <= idx < len(ids):
                selected.append(ids[idx])
        if selected:
            self.graph_selected_nodes = set(selected)
        return selected

    def on_graph_selection_change(self, _=None):
        self.graph_selected_nodes = set(self.get_graph_selected_nodes())
        self.update_graph_info_label()
        self.redraw_graph()

    def select_all_graph_nodes(self):
        if not hasattr(self, "graph_listbox"):
            return
        ids = sorted(self.nodes.keys())
        self.graph_listbox.selection_clear(0, "end")
        for idx in range(len(ids)):
            self.graph_listbox.selection_set(idx)
        self.graph_selected_nodes = set(ids)
        self.update_graph_info_label()
        self.redraw_graph()

    def select_current_graph_node(self):
        if not hasattr(self, "graph_listbox"):
            return
        node_id = self.selected_node.get()
        ids = sorted(self.nodes.keys())
        self.graph_listbox.selection_clear(0, "end")
        if node_id in ids:
            idx = ids.index(node_id)
            self.graph_listbox.selection_set(idx)
            self.graph_selected_nodes = {node_id}
        self.update_graph_info_label()
        self.redraw_graph()

    def clear_graph_selection(self):
        if not hasattr(self, "graph_listbox"):
            return
        self.graph_listbox.selection_clear(0, "end")
        self.graph_selected_nodes.clear()
        self.update_graph_info_label()
        self.redraw_graph()

    def clear_selected_graph(self):
        targets = self.get_graph_selected_nodes()
        if not targets:
            node_id = self.selected_node.get()
            targets = [node_id] if node_id else []
        if not targets:
            return
        if messagebox.askyesno(self.tr("clear_graph_title"), self.tr("clear_graph_question")):
            for node_id in targets:
                if node_id in self.nodes:
                    self.nodes[node_id]["rows"].clear()
            self.redraw_graph()

    def parse_axis_value(self, text, default=None):
        try:
            return float(str(text).strip().replace(",", "."))
        except Exception:
            return default

    def apply_manual_axes(self):
        self.graph_auto_scale.set(False)
        self.redraw_graph()

    def set_auto_axes(self):
        self.graph_auto_scale.set(True)
        self.redraw_graph()

    def apply_axis_limits(self):
        if self.graph_auto_scale.get():
            return

        t_min = self.parse_axis_value(self.temp_min_var.get())
        t_max = self.parse_axis_value(self.temp_max_var.get())
        p_min = self.parse_axis_value(self.press_min_var.get())
        p_max = self.parse_axis_value(self.press_max_var.get())

        if t_min is not None and t_max is not None and t_max > t_min:
            self.ax_t.set_ylim(t_min, t_max)

        if p_min is not None and p_max is not None and p_max > p_min:
            self.ax_p.set_ylim(p_min, p_max)

    def redraw_graph(self):
        if Figure is None or not hasattr(self, "ax_t"):
            return
        selected = self.get_graph_selected_nodes()

        self.graph_dirty=False
        self.ax_t.clear()
        self.ax_p.clear()
        self.refresh_graph_titles()

        any_data = False
        for node_id in selected:
            node = self.nodes.get(node_id)
            if not node:
                continue
            rows = node.get("rows", [])
            if not rows:
                continue
            any_data = True
            # Break the line on a new boot/session or a gap; use actual measurement time.
            plot_rows=[]
            for r in rows:
                if plot_rows:
                    previous=plot_rows[-1]
                    if previous is not None and (r.get("session")!=previous.get("session") or (r["dt"]-previous["dt"]).total_seconds()>max(5,previous.get("interval",60)*1.8)):
                        plot_rows.append(None)
                plot_rows.append(r)
            x=[r["dt"] if r else plot_rows[i-1]["dt"] for i,r in enumerate(plot_rows)]
            temps = [r["temp"] if r else float("nan") for r in plot_rows]
            presses = [r["press"] if r else float("nan") for r in plot_rows]
            name = node.get("name", node_id)
            color = self.node_color(node_id)
            t_label = f"{name}  {temps[-1]:.1f}°C"
            p_label = f"{name}  {presses[-1]:.1f} hPa"

            # light glow + main line + last-point marker for a more professional look
            self.ax_t.plot(x, temps, linewidth=6, alpha=0.10, color=color)
            self.ax_t.plot(x, temps, linewidth=2.4, label=t_label, color=color)
            self.ax_t.fill_between(x, temps, [min(temps)] * len(temps), color=color, alpha=0.06)
            self.ax_t.scatter([x[-1]], [temps[-1]], s=36, color=color, edgecolors="#ffffff", linewidths=0.6, zorder=5)

            self.ax_p.plot(x, presses, linewidth=6, alpha=0.10, color=color)
            self.ax_p.plot(x, presses, linewidth=2.4, label=p_label, color=color)
            self.ax_p.fill_between(x, presses, [min(presses)] * len(presses), color=color, alpha=0.05)
            self.ax_p.scatter([x[-1]], [presses[-1]], s=36, color=color, edgecolors="#ffffff", linewidths=0.6, zorder=5)

        if any_data:
            leg_t = self.ax_t.legend(loc="upper left", fontsize=8, frameon=True)
            leg_p = self.ax_p.legend(loc="upper left", fontsize=8, frameon=True)
            for leg in [leg_t, leg_p]:
                if leg:
                    frame = leg.get_frame()
                    frame.set_facecolor("#0a1220")
                    frame.set_edgecolor(COL["border"])
                    frame.set_alpha(0.95)
                for txt in leg.get_texts():
                    txt.set_color(COL["text"])

            if self.graph_auto_scale.get():
                self.ax_t.relim()
                self.ax_t.autoscale_view()
                self.ax_p.relim()
                self.ax_p.autoscale_view()
            else:
                self.apply_axis_limits()
        else:
            self.ax_t.text(0.5, 0.5, self.tr("graph_no_sel") if selected else self.tr("graph_none"),
                           transform=self.ax_t.transAxes, ha="center", va="center",
                           color=COL["muted"], fontsize=11)
            self.ax_p.text(0.5, 0.5, self.tr("graph_no_sel") if selected else self.tr("graph_none"),
                           transform=self.ax_p.transAxes, ha="center", va="center",
                           color=COL["muted"], fontsize=11)

        from matplotlib.dates import DateFormatter
        self.ax_t.xaxis.set_major_formatter(DateFormatter("%d.%m %H:%M"))
        self.ax_p.xaxis.set_major_formatter(DateFormatter("%d.%m %H:%M"))
        self.canvas_t.draw_idle()
        self.canvas_p.draw_idle()
        self.update_graph_info_label()


    def update_analysis(self):
        if not hasattr(self, "analysis_text"):
            return
        node_id = self.analysis_node.get() or self.selected_node.get()
        node = self.nodes.get(node_id)
        self.analysis_text.delete("1.0", "end")
        if not node or not node.get("rows"):
            self.analysis_text.insert("end", "Noch keine Daten für diese Node.\n")
            return
        rows = node["rows"]
        temps = [r["temp"] for r in rows]
        presses = [r["press"] for r in rows]
        last = rows[-1]
        span_s = 0
        if rows and rows[0]["dt"]:
            span_s = (rows[-1]["dt"] - rows[0]["dt"]).total_seconds()

        def dur(s):
            if s < 120: return f"{s:.0f} s"
            if s < 7200: return f"{s/60:.1f} min"
            if s < 172800: return f"{s/3600:.1f} h"
            return f"{s/86400:.2f} Tage"

        comfort = "angenehm"
        if last["temp"] < 18: comfort = "kalt"
        elif last["temp"] < 20: comfort = "kühl"
        elif last["temp"] > 27: comfort = "heiß"
        elif last["temp"] > 24: comfort = "warm"

        weather = "stabil"
        if len(rows) > 1:
            dp = rows[-1]["press"] - rows[0]["press"]
            if dp > 2: weather = "stark steigend"
            elif dp > 0.7: weather = "leicht steigend"
            elif dp < -2: weather = "stark fallend"
            elif dp < -0.7: weather = "leicht fallend"

        text = []
        text.append(f"Node:        {node_id}")
        text.append(f"Name:        {node.get('name', node_id)}")
        text.append(f"FW:          {node.get('fw','--')}")
        text.append(f"Mode:        {mode_name(node.get('mode','--'))}")
        text.append(f"Power:       {power_name(node.get('power','--'))}")
        text.append("")
        text.append(f"Temperatur:  {last['temp']:.2f} °C")
        text.append(f"T min/max:   {min(temps):.2f} / {max(temps):.2f} °C")
        text.append(f"T avg:       {sum(temps)/len(temps):.2f} °C")
        text.append(f"Komfort:     {comfort}")
        text.append("")
        text.append(f"Druck:       {last['press']:.2f} hPa")
        text.append(f"P min/max:   {min(presses):.2f} / {max(presses):.2f} hPa")
        text.append(f"P avg:       {sum(presses)/len(presses):.2f} hPa")
        text.append(f"Tendenz:     {weather}")
        text.append("")
        text.append(f"Messpunkte:  {len(rows)}")
        text.append(f"Zeitraum:    {dur(span_s)}")
        text.append(f"Letztes:     {node.get('last','--')}")
        text.append("")
        text.append("Nicht berechnet: Feuchtigkeit/Taupunkt/Schimmelrisiko ohne Feuchtesensor.")
        self.analysis_text.insert("end", "\n".join(text))

    def choose_pc_csv(self):
        path = filedialog.asksaveasfilename(defaultextension=".csv", filetypes=[("CSV","*.csv"),("Alle Dateien","*.*")])
        if not path:
            return
        if self.pc_csv_handle:
            self.pc_csv_handle.close()
        self.pc_csv_handle = open(path, "a", newline="", encoding="utf-8")
        self.pc_csv_writer = csv.writer(self.pc_csv_handle, delimiter=";")
        if self.pc_csv_handle.tell() == 0:
            self.pc_csv_writer.writerow(["pc_time","node_id","name","fw","log","raw_temp_c","temp_c","raw_pressure_hpa","pressure_hpa","temp_offset","press_offset","mode","power","wake","mac"])
        self.csv_label.config(text=path)
        if hasattr(self, "quick_csv_label"):
            short = path if len(path) <= 70 else '...' + path[-67:]
            self.quick_csv_label.config(text=short)
        self.memory_status_var.set(self.tr("csv_active"))

    def write_pc_csv(self, node_id, n):
        if not self.pc_csv_writer:
            return
        r = n["rows"][-1]
        self.pc_csv_writer.writerow([
            n["last"], node_id, n.get("name", node_id), n["fw"], r["log"],
            f'{r["raw_t"]:.2f}', f'{r["temp"]:.2f}',
            f'{r["raw_p"]:.2f}', f'{r["press"]:.2f}',
            f'{r["toff"]:.2f}', f'{r["poff"]:.2f}',
            r["mode"], r["power"], r["wake"], n["mac"]
        ])
        self.pc_csv_handle.flush()

    def dump_selected(self):
        node_id = self.selected_node.get()
        if not node_id:
            return
        self.send("dump local" if node_id==getattr(self,"local_node_id",None) else f"dump {node_id}")

    def write_term(self, text):
        self.term.insert("end",text)
        if int(self.term.index("end-1c").split(".")[0])>1500:
            self.term.delete("1.0","501.0")
        self.term.see("end")

    def on_close(self):
        if self.pc_csv_handle:
            self.pc_csv_handle.close()
        self.serial.disconnect()
        self.archive.commit(); self.archive.close()
        self.destroy()

def main():
    app = App()
    app.protocol("WM_DELETE_WINDOW", app.on_close)
    app.mainloop()

if __name__ == "__main__":
    main()
