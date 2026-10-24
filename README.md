# KC87 Pico Recorder

KC87 Kassetten-Recorder Emulator mit Raspberry Pi Pico (RP2040) oder Pico 2 (RP2350).

## Status

**✅ Recording (Aufnahme): funktional**  
❌ Playback (Wiedergabe): Noch nicht implementiert

## Projektstruktur

```
kc87_pico_recorder/
├── firmware/            Pico-Firmware (C, Pico SDK, PIO)
├── pc_commandline_tool/ PC-Kommandozeilentool kc87tool (C++17)
├── hardware/            KiCad-Schaltplan & PCB
├── doc/                 Dokumentation
├── PROTOCOL.md          Protokoll-Dokumentation
└── README.md
```

## Hardware

- Raspberry Pi Pico (RP2040) oder Pico 2 (RP2350), optional mit Debug-Probe
- **GPIO 2**: Eingang für das KC87-Signal (KC87 → Pico, Programm-Save)
- **GPIO 3**: Playback-Ausgang (`PLAY_SINGAL_PICO` — Pico → KC87), im Schaltplan vorgesehen, in der Firmware noch nicht implementiert

Maßgeblich für die Pinbelegung ist der Schaltplan in `hardware/`.

## Firmware

- Quellen: `firmware/main.c`, `firmware/pulse_timer.pio` (Target `kc87_logger`)
- Ein PIO-Zustandsautomat misst abwechselnd die Dauer der High- und Low-Phasen an GP2
- Timing-Auflösung: 0,2 µs (5 MHz Zähltakt, 32-Bit-Zähler)
- Die Messwerte laufen per DMA in einen 32-KB-Ringpuffer; daher gehen auch bei kurzen USB-Verzögerungen keine Pulse verloren
- Übertragung über USB-CDC (siehe [PROTOCOL.md](PROTOCOL.md))
- Gesteuert vom PC: `S` startet die Aufnahme, `X` beendet sie

### Protokoll (Kurzfassung)

Alle Wörter sind `uint32` little-endian:

| Richtung | Inhalt |
|----------|--------|
| PC → Pico | `'S'` Start, `'X'` Stopp |
| Pico → PC | Header: `"KC87"` (`0x3738434B`), `tick_hz` |
| Pico → PC | Daten: Dauer in Ticks, abwechselnd High, Low, High, … |
| Pico → PC | Ende: `0xFFFFFFFF`, Status, Anzahl Werte |

Status: Bit 0 = Ringpuffer-Überlauf, Bit 1 = PIO-Stall.

## PC-Tool: kc87tool

Quelle: `pc_commandline_tool/kc87tool.cpp` (Windows, Linux, macOS).

```bash
kc87tool record <port> [--sec 30] [--out aufnahme]   # z.B. COM5 oder /dev/ttyACM0
kc87tool analyze <datei.csv>                          # Statistik + SVG-Diagramme
kc87tool wav <datei.csv> [--rate 44100]               # Rechtecksignal als WAV
```

- **record**: sendet `S`, zeichnet `--sec` Sekunden auf (Standard 30), sendet `X` und speichert die Pulse als `<out>.csv`. Danach wird automatisch `analyze` ausgeführt. Meldet Ringpuffer-Überlauf, PIO-Stall oder fehlenden Endmarker als Warnung.
- **analyze**: gibt Min/Median/Max für High, Low und Periodendauer aus, sowie die häufigsten Periodenlängen. Schreibt `<name>_histogramm.svg` und `<name>_verlauf.svg` (im Browser öffnen).
- **wav**: erzeugt `<name>.wav` (8 Bit, Mono) aus der CSV-Datei.

CSV-Format: Kopfzeile `level;dauer_us`, danach je Zeile `1;<µs>` (High) oder `0;<µs>` (Low).

## Build

### Firmware

Über die VS Code Pico-Extension oder manuell (Pico SDK 2.3.0, `PICO_SDK_PATH` setzen):

```bash
cd firmware
mkdir -p build && cd build
cmake ..
ninja          # oder: make
```

Ergebnis: `kc87_logger.uf2` / `.elf`.

Die Ziel-Platine steht in `firmware/CMakeLists.txt` (`set(PICO_BOARD pico ...)`):

- Pico 1 (RP2040): `pico` (Standard)
- Pico 2 (RP2350): `pico2`, oder beim Konfigurieren mit `cmake -DPICO_BOARD=pico2 ..` (bei einem schon vorhandenen `build/`-Ordner diesen vorher löschen)

### PC-Tool

```bash
cd pc_commandline_tool
cmake -S . -B build
cmake --build build
```

Alternativ ohne CMake: `g++ -std=c++17 -O2 -o kc87tool kc87tool.cpp` (unter Windows mit MinGW/MSYS2 bzw. `cl /std:c++17 /O2 /EHsc kc87tool.cpp` mit Visual Studio).

## Workflow

1. **Pico flashen**: Firmware kompilieren → über Debug-Probe oder UF2 flashen
2. **Aufnahme starten**: `kc87tool record /dev/ttyACM0 --sec 30 --out aufnahme`
3. **Programm-Save am KC87 starten** (Signal an GPIO 2)
4. **Auswerten**: Die Analyse läuft nach der Aufnahme automatisch; später erneut mit `kc87tool analyze aufnahme.csv`

## Bekannte Einschränkungen

- Playback-Funktionalität ist noch nicht implementiert
- Die Aufnahmedauer wird vom PC festgelegt (`--sec`), es gibt keinen automatischen Stopp nach Inaktivität
- Der Ringpuffer fasst 8192 Messwerte; bei sehr hoher Pulsrate und blockierter USB-Verbindung kann er überlaufen (Status-Bit 0)
